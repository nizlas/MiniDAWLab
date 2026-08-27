#pragma once

#include <JuceHeader.h>

#include <functional>

#include "domain/Track.h"
#include "util/AsyncLifetimeToken.h"

class Session;
class Transport;
class InstrumentRuntimeCoordinator;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;

/// Arrangement-level MIDI import at the transport playhead (instrument track header → FileChooser).
class InstrumentMidiImportCoordinator final
{
public:
    struct Callbacks
    {
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableInstrumentEdit;
        std::function<void()> syncViewportFromSession;
        std::function<void()> refreshInstrumentUi;
    };

    InstrumentMidiImportCoordinator(Session& session,
                                    Transport& transport,
                                    InstrumentRuntimeCoordinator& instrumentRuntime,
                                    TrackLanesView& trackLanesView,
                                    TimelineRulerView& rulerView,
                                    InspectorView& inspectorView,
                                    Callbacks callbacks);

    void importMidiFileForInstrumentTrack(TrackId tid);

private:
    Session& session_;
    Transport& transport_;
    InstrumentRuntimeCoordinator& instrumentRuntime_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
    bool importInFlight_ = false;
    /// Stability Slice 4: FileChooser completions check this before touching the coordinator.
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentMidiImportCoordinator)
};
