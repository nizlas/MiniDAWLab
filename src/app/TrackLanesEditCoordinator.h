#pragma once

#include <JuceHeader.h>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "domain/PlacedClip.h"
#include "domain/SessionHistory.h"
#include "domain/Track.h"
#include "ui/ClipWaveformView.h"

class Session;
class PlaybackEngine;
class PluginInsertHost;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;
class ExperimentalInstrumentHost;
class InstrumentTrackController;

/// Owns the wiring of `TrackLanesView` edit-affordance callbacks: delete/reorder track,
/// move/trim/split clip (all undoable) plus the audio/instrument header active-mutex pair.
/// All bodies preserve the original `TransportControlsContent` semantics byte-for-byte.
class TrackLanesEditCoordinator
{
public:
    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;

        /// Routes through `UndoRedoCoordinator::executeUndoableSessionEdit` (same null-guard as before).
        std::function<void(const juce::String& label, std::function<bool()> mutator)>
            executeUndoableSessionEdit;

        /// Routes through `UndoRedoCoordinator::executeUndoableTrackDelete`: the mutator hands back
        /// the pre-teardown insert chain and (for instrument tracks) the captured project row so
        /// Delete Track is undoable for every track kind.
        std::function<void(
            const juce::String& label,
            std::function<bool(std::optional<PluginUndoStepSides>&,
                               std::optional<InstrumentTrackDeleteUndoSides>&)> mutator)>
            executeUndoableTrackDelete;

        std::function<void()> syncViewportFromSession;

        // Delete-track instrument branch hooks (must keep call order; see install()).
        std::function<ExperimentalInstrumentHost*(TrackId)> getInstrumentHostForTrack;
        std::function<InstrumentTrackController*(TrackId)> getInstrumentControllerForTrack;
        std::function<void(TrackId)> resetMidiEditorBookingIfOpenOnTrack;
        std::function<void(TrackId)> tearDownInstrumentTimelineUiForTrack;
        std::function<void(TrackId)> removeInstrumentRuntimeForTrack;
        std::function<void()> refreshInstrumentUi;

        /// Delete-Track undo restore: recreate host + controller keyed to the restored session row
        /// (same registry entry the project-load restore path uses).
        std::function<std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>(TrackId)>
            getOrCreateInstrumentRuntimeForTrack;
        /// Current device sample rate for musical length derivation on restored controllers.
        std::function<double()> getDeviceSampleRate;

        // UI-only header active mutex (audio header vs instrument-row).
        std::function<bool()> hasAnyKeyedInstrumentControllerActive;
        std::function<void()> deactivateKeyedInstrumentControllersOnly;

        /// Clears record-arm when the armed lane is removed (Group/Master cannot be armed from UI).
        std::function<void(TrackId)> disarmRecorderIfTrack;
    };

    TrackLanesEditCoordinator(Session& session,
                              PlaybackEngine& playbackEngine,
                              PluginInsertHost& pluginHost,
                              TrackLanesView& trackLanesView,
                              TimelineRulerView& rulerView,
                              InspectorView& inspectorView,
                              Callbacks callbacks);

    TrackLanesEditCoordinator(const TrackLanesEditCoordinator&) = delete;
    TrackLanesEditCoordinator& operator=(const TrackLanesEditCoordinator&) = delete;

    /// [Message thread] Registers the seven `TrackLanesView::setOn...` callbacks. Idempotent for
    /// the borrowed view (each setter overwrites). Call once from owner's ctor after all
    /// dependent coordinators (`UndoRedoCoordinator`, `InstrumentRuntimeCoordinator`,
    /// `InstrumentTimelineRowCoordinator`, `MidiEditorPresenter`) are constructed.
    void install();

    /// [Message thread] Undo of Delete Track (instrument): the session row is already restored from
    /// the timeline snapshot; recreate the runtime from the captured project row via the same
    /// restore path project load uses (realtime-gated; plugin reload may fall back to a
    /// placeholder shell when the plugin cannot be loaded).
    void restoreDeletedInstrumentTrackForUndo(const InstrumentTrackDeleteUndoSides& sides);

    /// [Message thread] Redo of Delete Track (instrument): re-run the hardened runtime teardown
    /// (editors closed, timeline UI removed, publish-before-destroy retire) for `tid`. The session
    /// row was already removed by the redo timeline snapshot.
    void redoTeardownDeletedInstrumentTrack(TrackId tid);

private:
    Session& session_;
    PlaybackEngine& playbackEngine_;
    PluginInsertHost& pluginHost_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
};
