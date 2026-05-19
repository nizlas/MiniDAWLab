#pragma once

#include <JuceHeader.h>

#include <functional>
#include <vector>

#include "domain/PlacedClip.h"
#include "domain/Track.h"
#include "ui/ClipWaveformView.h"

class Session;
class PluginInsertHost;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;
class ExperimentalInstrumentHost;

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

        std::function<void()> syncViewportFromSession;

        // Delete-track instrument branch hooks (must keep call order; see install()).
        std::function<ExperimentalInstrumentHost*(TrackId)> getInstrumentHostForTrack;
        std::function<void(TrackId)> resetMidiEditorBookingIfOpenOnTrack;
        std::function<void(TrackId)> tearDownInstrumentTimelineUiForTrack;
        std::function<void(TrackId)> removeInstrumentRuntimeForTrack;
        std::function<void()> refreshInstrumentUi;

        // UI-only header active mutex (audio header vs instrument-row).
        std::function<bool()> hasAnyKeyedInstrumentControllerActive;
        std::function<void()> deactivateKeyedInstrumentControllersOnly;

        /// Clears record-arm when the armed lane is removed (Group/Master cannot be armed from UI).
        std::function<void(TrackId)> disarmRecorderIfTrack;
    };

    TrackLanesEditCoordinator(Session& session,
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

private:
    Session& session_;
    PluginInsertHost& pluginHost_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
};
