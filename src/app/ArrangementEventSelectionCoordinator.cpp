#include "app/ArrangementEventSelectionCoordinator.h"

#include "app/InstrumentRuntimeCoordinator.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/TrackLanesView.h"

ArrangementEventSelectionCoordinator::ArrangementEventSelectionCoordinator(
    TrackLanesView& trackLanesView,
    InstrumentRuntimeCoordinator& instrumentRuntime) noexcept
    : trackLanesView_(trackLanesView)
    , instrumentRuntime_(instrumentRuntime)
{
}

void ArrangementEventSelectionCoordinator::clearAllArrangementEventSelections() noexcept
{
    trackLanesView_.clearAllPlacedClipSelections();
    instrumentRuntime_.forEachInstrumentController(
        [](TrackId, InstrumentTrackController& ctl) { ctl.clearClipSelection(); });
}

void ArrangementEventSelectionCoordinator::clearAllInstrumentControllerSelectionsOnly() noexcept
{
    instrumentRuntime_.forEachInstrumentController(
        [](TrackId, InstrumentTrackController& ctl) { ctl.clearClipSelection(); });
}

void ArrangementEventSelectionCoordinator::clearAudioAndOtherInstrumentControllerSelections(
    const TrackId keepInstrumentTrackId) noexcept
{
    trackLanesView_.clearAllPlacedClipSelections();
    instrumentRuntime_.forEachInstrumentController(
        [keepInstrumentTrackId](const TrackId tid, InstrumentTrackController& ctl) {
            if (tid != keepInstrumentTrackId)
            {
                ctl.clearClipSelection();
            }
        });
}
