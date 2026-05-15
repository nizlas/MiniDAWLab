#pragma once

#include <JuceHeader.h>

#include "domain/Track.h"

#include <functional>

class TrackLanesView;
class InstrumentRuntimeCoordinator;

/// UI-only arrangement selection bridge between placed audio clips and instrument MIDI clips.
/// Runs on the message thread only; does not mutate `Session` or push undo steps.
class ArrangementEventSelectionCoordinator final
{
public:
    ArrangementEventSelectionCoordinator(TrackLanesView& trackLanesView,
                                         InstrumentRuntimeCoordinator& instrumentRuntime) noexcept;

    void clearAllArrangementEventSelections() noexcept;
    void clearAllInstrumentControllerSelectionsOnly() noexcept;
    void clearAudioAndOtherInstrumentControllerSelections(TrackId keepInstrumentTrackId) noexcept;

private:
    TrackLanesView& trackLanesView_;
    InstrumentRuntimeCoordinator& instrumentRuntime_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ArrangementEventSelectionCoordinator)
};
